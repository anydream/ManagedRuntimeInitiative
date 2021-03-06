/*
 * Copyright 2001 Sun Microsystems, Inc.  All Rights Reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Sun Microsystems, Inc., 4150 Network Circle, Santa Clara,
 * CA 95054 USA or visit www.sun.com if you need additional information or
 * have any questions.
 */

/* @test
 * @bug 4387038
 * @summary Ensure that java.rmi.Naming.lookup functions properly for names
 *          containing embedded ':' characters.
 */

import java.rmi.Naming;
import java.rmi.registry.LocateRegistry;
import java.rmi.registry.Registry;

public class LookupNameWithColon {
    public static void main(String[] args) throws Exception {
        String[] names = {
            "fairly:simple", "somewhat:more/complicated",
            "multiple:colons:in:name"
        };

        Registry reg;
        try {
            reg = LocateRegistry.createRegistry(Registry.REGISTRY_PORT);
        } catch (Exception ex) {
            reg = LocateRegistry.getRegistry();
        }
        for (int i = 0; i < names.length; i++) {
            reg.rebind(names[i], reg);
            Naming.lookup("rmi://localhost/" + names[i]);
        }
    }
}
